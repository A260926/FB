#include "spirv_glsl.hpp"
#include <cstdio>
#include <cstddef>
using namespace spirv_cross;
struct Probe : CompilerGLSL { using CompilerGLSL::CompilerGLSL; };
int main(){
  printf("Compiler size=%zu\n", sizeof(Compiler));
  printf("GLSL size=%zu\n", sizeof(CompilerGLSL));
  // check the 0x878 and 0x360 offsets
  printf("0x878=%d 0x360=%d\n", 0x878, 0x360);
  return 0;
}
