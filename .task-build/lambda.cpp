#include "spirv_glsl.hpp"
#include <cstdio>
using namespace spirv_cross;
int main(){
  printf("sizeof std::function<void()> = %zu\n", sizeof(std::function<void()>));
  printf("sizeof Vector<std::function<void()>> = %zu\n", sizeof(Vector<std::function<void()>>));
  return 0;
}
