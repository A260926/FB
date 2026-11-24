#include "fixture.hpp"
#include <iostream>
int main(){
    auto f = pair_fixture::make(2,3,true,true,true);
    spirv_cross::CompilerGLSL c(f.words);
    c.set_common_options(pair_fixture::options());
    c.build_combined_image_samplers();
    std::cout << c.compile();
    return 0;
}
