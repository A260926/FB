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
  // Where do hooks get added? Check constant arrays and LUTs
  c.ir.for_each_typed_id<SPIRConstant>([&](uint32_t, SPIRConstant& cc){
    if(cc.is_used_as_lut) fprintf(stderr,"LUT const %u subconst=%zu\n",cc.self,cc.subconstants.size());
  });
  // Count all output storage vars globally
  size_t outcount=0;
  c.ir.for_each_typed_id<SPIRVariable>([&](uint32_t, SPIRVariable& var){
    if(var.storage==StorageClassOutput) { outcount++; fprintf(stderr,"OUT var %u init=%u\n",var.self,var.initializer); }
  });
  fprintf(stderr,"outcount=%zu\n",outcount);
  return 0;
}
