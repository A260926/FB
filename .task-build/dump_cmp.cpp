#include "glsl_optimizer.h"
#include <cstdio>
#include <cstring>
int main(int argc, char** argv){
   glslopt_ctx* ctx = glslopt_initialize(kGlslTargetOpenGL);
   const char* shaders[][2] = {
   {"v", "#version 450 core\nlayout(location=0) in vec4 a; layout(location=1) in vec4 b;\nvoid main(){ gl_Position=(a*b)+a; }\n"},
   {"f", "#version 450 core\nuniform vec4 a,b,c; layout(location=0) out vec4 o;\nvoid main(){ o=(a*b)+(a*c); }\n"},
   {"loop", "#version 450 core\nuniform float a; uniform int n; layout(location=0) out float o;\nvoid main(){ float s=0.0; for(int i=0;i<n;i++){ s=s+a; } o=s; }\n"},
   };
   for (auto& sh : shaders) {
      glslopt_shader_type t = sh[0][0]=='v' ? kGlslOptShaderVertex : kGlslOptShaderFragment;
      glslopt_shader* s = glslopt_optimize(ctx, t, sh[1], 0);
      std::printf("@@@%s\n%s\n", sh[0], glslopt_get_output(s));
      glslopt_shader_delete(s);
   }
   glslopt_cleanup(ctx);
}
