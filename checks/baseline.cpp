#include "hlsl2glsl.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <cctype>
int main(){
 assert(Hlsl2Glsl_Initialize());
 const char*s="sampler2D maps[3]; float index; float4 main(float2 uv:TEXCOORD0):COLOR { return tex2D(maps[(int)index],uv); }";
 ShHandle h=Hlsl2Glsl_ConstructCompiler(EShLangFragment);
 int p=Hlsl2Glsl_Parse(h,s,ETargetGLSL_120,0,0); if(!p)std::puts(Hlsl2Glsl_GetInfoLog(h));assert(p);
 int t=Hlsl2Glsl_Translate(h,"main",ETargetGLSL_120,0); if(!t)std::puts(Hlsl2Glsl_GetInfoLog(h));assert(t);
 const char*out=Hlsl2Glsl_GetShader(h);assert(out&&std::strstr(out,"maps[")&&std::strstr(out,"texture2D"));std::puts(out);
 Hlsl2Glsl_DestructCompiler(h);
 std::ifstream fi("tests/fragment/array-const-in.txt"),fe("tests/fragment/array-const-out.txt");std::stringstream si,se;si<<fi.rdbuf();se<<fe.rdbuf();assert(!si.str().empty()&&!se.str().empty());
 h=Hlsl2Glsl_ConstructCompiler(EShLangFragment);assert(Hlsl2Glsl_Parse(h,si.str().c_str(),ETargetGLSL_110,0,0));assert(Hlsl2Glsl_Translate(h,"main",ETargetGLSL_110,0));
 auto norm=[](std::string x){x.erase(std::remove_if(x.begin(),x.end(),[](unsigned char c){return std::isspace(c);}),x.end());return x;};
 assert(norm(Hlsl2Glsl_GetShader(h))==norm(se.str()));std::puts("OLD_ARRAY_FIXTURE_OK");
 std::puts("DYNAMIC_SAMPLER_EMITTED");std::puts("BASELINE_OK");Hlsl2Glsl_DestructCompiler(h);Hlsl2Glsl_Shutdown();
}
