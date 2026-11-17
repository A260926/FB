#include "glsl_optimizer.h"
#include <cstdio>
int main(){
   glslopt_ctx* ctx = glslopt_initialize(kGlslTargetOpenGL);
   const char* precise =
      "#version 450 core\nuniform vec4 a,b,c; layout(location=0) out vec4 o;\n"
      "void main(){ precise vec4 r=(a*b)+(a*c); o=r; }\n";
   glslopt_shader* s = glslopt_optimize(ctx, kGlslOptShaderFragment, precise, 0);
   std::puts(glslopt_get_output(s));
   glslopt_shader_delete(s);
   glslopt_cleanup(ctx);
}
