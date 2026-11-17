#include "glsl_optimizer.h"
#include <cstdio>
int main(){
   glslopt_ctx* ctx = glslopt_initialize(kGlslTargetOpenGL);
   const char* src =
      "#version 450 core\nuniform float a; uniform int n; layout(location=0) out float o;\n"
      "void main(){ float s=0.0; for(int i=0;i<n;i++){ s=(s+a)*a; } precise float r=s; o=r; }\n";
   glslopt_shader* s = glslopt_optimize(ctx, kGlslOptShaderFragment, src, 0);
   std::printf("long-loop status=%d\n%s\n", glslopt_get_status(s), glslopt_get_output(s));
   glslopt_shader_delete(s);
   glslopt_cleanup(ctx);
}
