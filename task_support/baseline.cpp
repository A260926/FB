#include "HLSLParser.h"
#include "GLSLGenerator.h"
#include <cstdio>
#include <cstring>
#include <string>
#include <regex>
using namespace M4;
static bool translate(const char* source, const char* name, std::string& result) {
    Allocator a;
    HLSLTree tree(&a);
    HLSLParser parser(&a,name,source,strlen(source));
    if (!parser.Parse(&tree)) return false;
    GLSLGenerator gen;
    if (!gen.Generate(&tree,GLSLGenerator::Target_FragmentShader,GLSLGenerator::Version_140,"shade")) return false;
    result=gen.GetResult(); return !result.empty();
}
int main() {
    std::string good,broken;
    const char* control="float4 shade(float2 uv : TEXCOORD0) : COLOR0 { return float4(uv,0.0,1.0); }";
    const char* repro="float4 shade(float2 uv : TEXCOORD0) : COLOR0 { float3x3 m=float3x3(1,2,3,4,5,6,7,8,9); m[1]=float3(10,20,30); float2 v=m._m00_m12; return float4(v,m[1].x,1); }";
    if (!translate(control,"control.hlsl",good) || good.find("void main")==std::string::npos) return 2;
    puts("BASELINE_TRANSLATE_OK");
    if (!translate(repro,"matrix-repro.hlsl",broken)) return 3;
    FILE* f=fopen(".task-build/matrix-repro.glsl","wb");
    if (!f) return 4;
    fwrite(broken.data(),1,broken.size(),f); fclose(f);
    bool row=std::regex_search(broken,std::regex("matrix_row[0-9]*\\(m,1\\) ="));
    bool swizzle=broken.find("(m)[0][0][2][1]")!=std::string::npos;
    printf("MATRIX_REPRO row_lvalue_helper=%d chained_swizzle=%d\n",row,swizzle);
    return 0;
}
