#include "glsl_optimizer.h"
#include <cstdio>
int main(){
   glslopt_ctx* ctx = glslopt_initialize(kGlslTargetOpenGL);
   const char* src =
      "#version 450 core\nuniform vec4 a,b; layout(location=0) out vec4 o;\n"
      "void main(){ vec4 x=a*b; vec4 y=x.zywx; precise vec4 r=-y+a; o=r; }\n";
   glslopt_shader* s = glslopt_optimize(ctx, kGlslOptShaderFragment, src, 0);
   std::puts(glslopt_get_output(s));
   glslopt_shader_delete(s);
   glslopt_cleanup(ctx);
}
