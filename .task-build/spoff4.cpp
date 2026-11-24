#include "spirv_glsl.hpp"
#include <cstdio>
#include <cstddef>
using namespace spirv_cross;
int main(){
  printf("VectorView sizeof=%zu\n", sizeof(VectorView<std::function<void()>>));
  printf("  ptr off=0x%zx buffer_size off=0x%zx\n",
         offsetof(VectorView<std::function<void()>>, ptr),
         offsetof(VectorView<std::function<void()>>, buffer_size));
  return 0;
}
