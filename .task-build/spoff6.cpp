#include "spirv_glsl.hpp"
#include <cstdio>
#include <cstddef>
using namespace spirv_cross;
int main(){
  printf("sizeof SPIRFunction=%zu\n", sizeof(SPIRFunction));
  // Walk member offsets by re-declaring order from header (public ones)
  printf("  return_type off=0x%zx\n", offsetof(SPIRFunction, return_type));
  printf("  function_control off=0x%zx\n", offsetof(SPIRFunction, function_control));
  printf("  function_type off=0x%zx\n", offsetof(SPIRFunction, function_type));
  printf("  self off=0x%zx\n", offsetof(SPIRFunction, self));
  printf("  op off=0x%zx\n", offsetof(SPIRFunction, op));
  printf("  blocks off=0x%zx\n", offsetof(SPIRFunction, blocks));
  printf("  local_variables off=0x%zx\n", offsetof(SPIRFunction, local_variables));
  printf("  add_local off=0x%zx\n", offsetof(SPIRFunction, add_local_variable));
  printf("  parameter off=0x%zx\n", offsetof(SPIRFunction, parameter));
  printf("  shadow_arguments off=0x%zx\n", offsetof(SPIRFunction, shadow_arguments));
  printf("  combined_parameters off=0x%zx\n", offsetof(SPIRFunction, combined_parameters));
  printf("  entry_block off=0x%zx\n", offsetof(SPIRFunction, entry_block));
  printf("  fixup_hooks_out off=0x%zx\n", offsetof(SPIRFunction, fixup_hooks_out));
  printf("  fixup_hooks_in off=0x%zx\n", offsetof(SPIRFunction, fixup_hooks_in));
  return 0;
}
