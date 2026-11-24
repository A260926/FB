#include "spirv_glsl.hpp"
#include <cstdio>
using namespace spirv_cross;
int main(){
 printf("sizeof Compiler=%zu\n", sizeof(Compiler));
 printf("sizeof SPIRFunction=%zu\n", sizeof(SPIRFunction));
 printf("sizeof CombinedImageSamplerParameter=%zu\n", sizeof(SPIRFunction::CombinedImageSamplerParameter));
 printf("sizeof CompilerGLSL=%zu\n", sizeof(CompilerGLSL));
 return 0;
}
