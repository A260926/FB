#include "glsl_optimizer.h"
#include <cstdio>
#include <cstring>

int main() {
   const char* src =
      "#version 450 core\nuniform vec4 a,b,c; layout(location=0) out vec4 o;\n"
      "void main(){ precise vec4 r=(a*b)+c; o=r; }\n";
   glslopt_target targets[] = {kGlslTargetOpenGL, kGlslTargetMetal};
   const char* names[] = {"GL", "Metal"};
   for (int i = 0; i < 2; i++) {
      glslopt_ctx* ctx = glslopt_initialize(targets[i]);
      glslopt_shader* s = glslopt_optimize(ctx, kGlslOptShaderFragment, src, 0);
      std::printf("==== %s status=%d\n%s\n", names[i], glslopt_get_status(s),
                  glslopt_get_status(s) ? glslopt_get_output(s) : glslopt_get_log(s));
      glslopt_shader_delete(s);
      glslopt_cleanup(ctx);
   }
   // ES target with ES source: precise is parsed but ES output emits no keyword
   glslopt_ctx* esctx = glslopt_initialize(kGlslTargetOpenGLES30);
   const char* es =
      "#version 310 es\nprecision highp float;\nuniform highp vec4 a,b,c; layout(location=0) out highp vec4 o;\n"
      "void main(){ precise highp vec4 r=(a*b)+c; o=r; }\n";
   glslopt_shader* s = glslopt_optimize(esctx, kGlslOptShaderFragment, es, 0);
   std::printf("==== ES30 status=%d\n%s\n", glslopt_get_status(s),
               glslopt_get_status(s) ? glslopt_get_output(s) : glslopt_get_log(s));
   glslopt_shader_delete(s);
   glslopt_cleanup(esctx);
   return 0;
}
