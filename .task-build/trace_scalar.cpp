#include "fixture.hpp"
#include <iostream>
int main(int argc,char**argv){
    int mode=argc>1?atoi(argv[1]):0;
    auto f=pair_fixture::make(mode?3:1,1,mode!=0,false);
    spirv_cross::CompilerGLSL c(f.words);c.set_common_options(pair_fixture::options());
    c.build_combined_image_samplers();
    std::cout<<c.compile().size()<<"\n";
    return 0;
}
