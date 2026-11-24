#include "spirv_glsl.hpp"
#include <cstdio>
using namespace spirv_cross;
int main(){
  printf("VectorView<function> sizeof=%zu\n", sizeof(VectorView<std::function<void()>>));
  printf("SmallVector<function,8> sizeof=%zu\n", sizeof(SmallVector<std::function<void()>>));
  return 0;
}
