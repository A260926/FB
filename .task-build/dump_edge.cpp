#include "glsl_optimizer.h"
#include <cstdio>
int main(){
   glslopt_ctx* ctx = glslopt_initialize(kGlslTargetOpenGL);
   struct { const char* tag; const char* src; } cases[] = {
   {"ssbo", "#version 450 core\nuniform vec4 a,b; layout(binding=0) buffer B { vec4 o; } bb;\nvoid main(){ precise vec4 r=a*b; bb.o=r; }\n"},
   {"nan-cancel", "#version 450 core\nuniform vec4 a,b; layout(location=0) out vec4 o;\nvoid main(){ precise vec4 r=(a-a)+b; o=r; }\n"},
   {"chain5", "#version 450 core\nuniform vec4 a; layout(location=0) out vec4 o;\nvoid main(){ precise vec4 r=((((a+a)-a)*a)/a); o=r; }\n"},
   {"precise-input", "#version 450 core\nlayout(location=0) precise in vec4 a; layout(location=1) in vec4 b; layout(location=0) out vec4 o;\nvoid main(){ o=a*b; }\n"},
   };
   for (auto& c : cases) {
      glslopt_shader* s = glslopt_optimize(ctx, kGlslOptShaderFragment, c.src, 0);
      std::printf("== %s status=%d\n%s\n", c.tag, glslopt_get_status(s), glslopt_get_status(s)?glslopt_get_output(s):glslopt_get_log(s));
      glslopt_shader_delete(s);
   }
   glslopt_cleanup(ctx);
}
