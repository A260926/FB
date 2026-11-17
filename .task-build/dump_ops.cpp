#include "glsl_optimizer.h"
#include <cstdio>
int main(){
   glslopt_ctx* ctx = glslopt_initialize(kGlslTargetOpenGL);
   struct { const char* tag; const char* src; } cases[] = {
   {"sub", "#version 450 core\nuniform vec4 a,b; layout(location=0) out vec4 o;\nvoid main(){ precise vec4 r=a-b; o=r; }\n"},
   {"div", "#version 450 core\nuniform vec4 a,b; layout(location=0) out vec4 o;\nvoid main(){ precise vec4 r=a/b; o=r; }\n"},
   {"ctor-swz", "#version 450 core\nuniform vec4 a,b; layout(location=0) out vec4 o;\nvoid main(){ precise vec4 r=vec4(a.x,b.y,a.z,b.w); o=r; }\n"},
   {"tex", "#version 450 core\nuniform sampler2D t; uniform vec2 uv; uniform vec4 k; layout(location=0) out vec4 o;\nvoid main(){ precise vec4 r=texture(t,uv)+k; o=r; }\n"},
   {"nested-helper", "#version 450 core\nuniform vec4 a,b; layout(location=0) out vec4 o;\nvec4 g(vec4 x){ return x*b; } vec4 f(vec4 x){ return g(x)+a; } void main(){ precise vec4 r=f(a); o=r; }\n"},
   };
   for (auto& c : cases) {
      glslopt_shader* s = glslopt_optimize(ctx, kGlslOptShaderFragment, c.src, 0);
      std::printf("== %s status=%d\n%s\n", c.tag, glslopt_get_status(s), glslopt_get_output(s));
      glslopt_shader_delete(s);
   }
   glslopt_cleanup(ctx);
}
