#include "spirv_glsl.hpp"
#include <cstdio>
#include <cstddef>
using namespace spirv_cross;
struct Probe : CompilerGLSL {
  using CompilerGLSL::CompilerGLSL;
  static void offs() {
    printf("Compiler size=%zu CompilerGLSL=%zu\n", sizeof(Compiler), sizeof(CompilerGLSL));
  }
};
int main(){
  printf("SPIRFunction size=%zu\n", sizeof(SPIRFunction));
  printf("  fixup_hooks_in off=%zu\n", offsetof(SPIRFunction, fixup_hooks_in));
  printf("  fixup_hooks_out off=%zu\n", offsetof(SPIRFunction, fixup_hooks_out));
  printf("  combined_parameters off=%zu\n", offsetof(SPIRFunction, combined_parameters));
  printf("  shadow_arguments off=%zu\n", offsetof(SPIRFunction, shadow_arguments));
  Probe::offs();
  return 0;
}
