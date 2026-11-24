#include "spirv_hlsl.hpp"
#include "spirv_msl.hpp"
#include "fixture.hpp"
#include <iostream>
using namespace spirv_cross;
static void attempt(const char* name, int backend, int es, int vulkan, int ver){
  auto f=pair_fixture::make(2,3,true,true,false);
  try{
    if(backend==0){
      CompilerGLSL c(f.words);
      CompilerGLSL::Options o; o.es=es; o.vulkan_semantics=vulkan; o.version=ver;
      c.set_common_options(o);
      c.build_combined_image_samplers(); c.compile();
      std::cout<<name<<": NO THROW (bad)\n";
    } else if(backend==1){
      CompilerHLSL c(f.words); c.build_combined_image_samplers(); c.compile();
      std::cout<<name<<": HLSL NO THROW (bad)\n";
    } else {
      CompilerMSL c(f.words); c.build_combined_image_samplers(); c.compile();
      std::cout<<name<<": MSL NO THROW (bad)\n";
    }
  }catch(const CompilerError&){ std::cout<<name<<": threw OK\n"; }
}
int main(){
  attempt("desktop-glsl430",0,0,0,430);
  attempt("desktop-glsl420",0,0,0,420);
  attempt("es-glsl",0,1,0,300);
  attempt("vulkan-glsl450",0,0,1,450);
  attempt("hlsl",1,0,0,0);
  attempt("msl",2,0,0,0);
  return 0;
}
