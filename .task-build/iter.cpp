#include "fixture.hpp"
#include <iostream>
#include <cstdio>
int main(){
    for(int iter=0; iter<200; ++iter){
      auto f=pair_fixture::make(1,1,false,false);
      spirv_cross::CompilerGLSL c(f.words);c.set_common_options(pair_fixture::options());
      c.build_combined_image_samplers();
      volatile size_t s=c.compile().size();
      (void)s;
    }
    std::cerr<<"200 iters ok\n";
    return 0;
}
