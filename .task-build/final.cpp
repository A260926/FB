#include "fixture.hpp"
#include <iostream>
int main(int c,char**v){
  unsigned n=std::stoul(v[1]),m=std::stoul(v[2]);
  bool ia=std::string(v[3])!="0", sa=std::string(v[4])!="0", h=std::string(v[5])!="0";
  auto f=pair_fixture::make(n,m,ia,sa,h);
  spirv_cross::CompilerGLSL cc(f.words);cc.set_common_options(pair_fixture::options());
  cc.build_combined_image_samplers();
  auto id=cc.get_combined_image_samplers()[0].combined_id;
  cc.set_name(id,"paired");cc.set_decoration(id,spv::DecorationBinding,7);
  std::cout<<cc.compile();
  return 0;
}
