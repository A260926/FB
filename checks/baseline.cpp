#include "HLSLParser.h"
#include "GLSLGenerator.h"
#include <cstdio>
#include <cstring>
int main(){using namespace M4; const char* s="cbuffer Params { float2 uv; float fog; float4 tint[2]; }; float4 main(float2 p:TEXCOORD0):COLOR0 { return tint[1] + float4(uv,fog,1); }"; Allocator a; HLSLTree t(&a); HLSLParser p(&a,"baseline.hlsl",s,strlen(s)); if(!p.Parse(&t))return 1; GLSLGenerator g; GLSLGenerator::Options o; o.flags=GLSLGenerator::Flag_EmulateConstantBuffer; if(!g.Generate(&t,GLSLGenerator::Target_FragmentShader,GLSLGenerator::Version_140,"main",o))return 2; puts(g.GetResult()); if(!strstr(g.GetResult(),"uniform vec4 Params[3]"))return 3; puts("BASELINE_CBUFFER_OK");}
