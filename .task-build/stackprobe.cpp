#include "fixture.hpp"
#include <iostream>
#include <cstdio>
#include <cstdint>
static volatile char* sink;
static char* base=nullptr;
static void probe(const char* where){ char x; if(!base) base=&x; long used=base-&x; if(used>200000) fprintf(stderr,"STACK %s used=%ld\n",where,used); }
int main(){
  char b; base=&b;
  auto f=pair_fixture::make(1,1,false,false);
  spirv_cross::CompilerGLSL c(f.words);c.set_common_options(pair_fixture::options());
  c.build_combined_image_samplers();
  probe("after build");
  size_t s=c.compile().size();
  probe("after compile");
  std::cerr<<"size="<<s<<"\n";
  return 0;
}
