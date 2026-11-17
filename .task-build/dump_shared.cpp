#include "glsl_optimizer.h"
#include <cstdio>
int main(){
   glslopt_ctx* ctx = glslopt_initialize(kGlslTargetOpenGL);
   const char* src =
      "#version 450 core\nuniform vec4 a,b,c;\n"
      "layout(location=0) out vec4 p; layout(location=1) out vec4 q;\n"
      "void main(){ precise vec4 r=(a*b)+c; p=r; q=(a*b)+(a*c); }\n";
   glslopt_shader* s = glslopt_optimize(ctx, kGlslOptShaderFragment, src, 0);
   std::puts(glslopt_get_output(s));
   glslopt_shader_delete(s);
   glslopt_cleanup(ctx);
}
