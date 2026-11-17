#include "glsl_optimizer.h"
#include <cstdio>
int main(){
   glslopt_ctx* ctx = glslopt_initialize(kGlslTargetOpenGL);
   const char* cases[][2] = {
      {"addchain", "#version 450 core\nuniform vec4 a,b,c; layout(location=0) out vec4 o;\nvoid main(){ o=(a+b)+c; }\n"},
      {"addchain4", "#version 450 core\nuniform vec4 a,b,c,d; layout(location=0) out vec4 o;\nvoid main(){ o=((a+b)+c)+d; }\n"},
      {"distribute", "#version 450 core\nuniform vec4 a,b,c; layout(location=0) out vec4 o;\nvoid main(){ o=(a*b)+(a*c); }\n"},
      {"fmastyle", "#version 450 core\nuniform vec4 a,b,c; layout(location=0) out vec4 o;\nvoid main(){ o=(a*b)+c; }\n"},
   };
   for (auto& t : cases) {
      glslopt_shader* s = glslopt_optimize(ctx, kGlslOptShaderFragment, t[1], 0);
      std::printf("== %s\n%s\n", t[0], glslopt_get_output(s));
      glslopt_shader_delete(s);
   }
   glslopt_cleanup(ctx);
}
