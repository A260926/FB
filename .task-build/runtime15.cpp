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
  // list ALL SPIRVariables with storage and whether a constant initializer
  size_t tot=0;
  c.ir.for_each_typed_id<SPIRVariable>([&](uint32_t, SPIRVariable& var){
    tot++;
    auto *cc=c.maybe_get<SPIRConstant>(var.initializer);
    if(var.initializer && cc)
      fprintf(stderr,"INIT var %u storage=%d init=%u\n",var.self,(int)var.storage,var.initializer);
  });
  fprintf(stderr,"total vars=%zu\n",tot);
  // Also check private storage vars (LUTs live there)
  size_t priv=0;
  c.ir.for_each_typed_id<SPIRVariable>([&](uint32_t, SPIRVariable& var){
    if(var.storage==StorageClassPrivate){priv++; fprintf(stderr,"PRIV var %u init=%u static=%u\n",var.self,var.initializer,var.static_expression);}
  });
  fprintf(stderr,"private=%zu\n",priv);
  return 0;
}
