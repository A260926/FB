#include "spirv_glsl.hpp"
#include <cstdio>
#include <cstddef>
using namespace spirv_cross;
struct Probe : CompilerGLSL {
  static void show(){
    // can't offsetof protected; use dummy member probes via inheritance
  }
};
int main(){
  // fixup_hooks_in offset in SPIRFunction
  printf("SPIRFunction::fixup_hooks_in = %zu (0x%zx)\n",
         offsetof(SPIRFunction, fixup_hooks_in), offsetof(SPIRFunction, fixup_hooks_in));
  return 0;
}
