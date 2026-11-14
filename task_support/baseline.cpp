#include "hlsl2glsl.h"
#include <cstdio>
#include <string>
#include <cstring>
struct Includes { int opened; int closed; Includes():opened(0),closed(0){} };
static bool C_DECL open_file(bool, const char* name, const char*, const char*, std::string& out, void* data) {
    Includes* state=static_cast<Includes*>(data);
    if (!strcmp(name,"outer.h")) out="#line 10 \"outer.h\"\n#include \"inner.h\"\n#define APPLY(v) WRAP(v)\n";
    else if (!strcmp(name,"inner.h")) out="#line 70 \"inner.h\"\n#define BAD(v) missing_component(v)\n#define WRAP(v) BAD(v)\n";
    else return false;
    ++state->opened; return true;
}
static void C_DECL close_file(const char*,void* data) { ++static_cast<Includes*>(data)->closed; }
int main() {
    if (!Hlsl2Glsl_Initialize()) return 1;
    ShHandle good=Hlsl2Glsl_ConstructCompiler(EShLangFragment);
    const char* source="uniform float4 gain; float4 main(float2 uv:TEXCOORD0):COLOR { return uv.xxxx * gain; }";
    if (!Hlsl2Glsl_Parse(good,source,ETargetGLSL_120,0,0) || !Hlsl2Glsl_Translate(good,"main",ETargetGLSL_120,0)) return 2;
    const char* shader=Hlsl2Glsl_GetShader(good);
    if (!shader || !strstr(shader,"void main") || Hlsl2Glsl_GetUniformCount(good)!=1) return 3;
    puts("BASELINE_SHADER_OK uniforms=1");
    ShHandle bad=Hlsl2Glsl_ConstructCompiler(EShLangFragment);
    Includes state;
    Hlsl2Glsl_ParseCallbacks callbacks={open_file,close_file,&state};
    const char* broken="#line 40 \"material.hlsl\"\n#include \"outer.h\"\nfloat4 main(float2 uv:TEXCOORD0):COLOR { return APPLY(uv); }\n";
    int accepted=Hlsl2Glsl_Parse(bad,broken,ETargetGLSL_120,&callbacks,0);
    const char* diagnostic=Hlsl2Glsl_GetInfoLog(bad);
    printf("NESTED_MACRO_REPRO rejected=%d opens=%d closes=%d\n",!accepted,state.opened,state.closed);
    puts("INFOLOG_BEGIN"); puts(diagnostic?diagnostic:""); puts("INFOLOG_END");
    int ok=!accepted && state.opened==2 && state.closed==2 && diagnostic && strstr(diagnostic,"missing_component");
    Hlsl2Glsl_DestructCompiler(good);Hlsl2Glsl_DestructCompiler(bad);Hlsl2Glsl_Shutdown();
    return ok?0:4;
}
