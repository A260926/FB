#define protected public
#define private public
#include "spirv_glsl.hpp"
#include <cstdio>
#include <cstddef>
using namespace spirv_cross;
int main(){
  printf("sizeof SPIRFunction=%zu (0x%zx)\n", sizeof(SPIRFunction), sizeof(SPIRFunction));
  printf("  shadow_arguments off=0x%zx\n", offsetof(SPIRFunction, shadow_arguments));
  printf("  combined_parameters off=0x%zx\n", offsetof(SPIRFunction, combined_parameters));
  printf("  fixup_hooks_out off=0x%zx\n", offsetof(SPIRFunction, fixup_hooks_out));
  printf("  fixup_hooks_in off=0x%zx\n", offsetof(SPIRFunction, fixup_hooks_in));
  return 0;
}
