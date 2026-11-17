#include "glsl_optimizer.h"
#include <cstdio>
#include <cstring>

static void run(glslopt_ctx* ctx, const char* tag, const char* src, glslopt_shader_type st = kGlslOptShaderFragment) {
   glslopt_shader* s = glslopt_optimize(ctx, st, src, 0);
   std::printf("\n======== %s status=%d\n", tag, glslopt_get_status(s));
   if (glslopt_get_status(s)) std::puts(glslopt_get_output(s));
   else std::puts(glslopt_get_log(s));
   glslopt_shader_delete(s);
}

int main() {
   glslopt_ctx* ctx = glslopt_initialize(kGlslTargetOpenGL);

   run(ctx, "precise-out-qualifier",
      "#version 450 core\nuniform vec4 a,b,c;\n"
      "layout(location=0) precise out vec4 o;\n"
      "void main(){ o=(a*b)+c; }\n");

   run(ctx, "unrollable-loop",
      "#version 450 core\nuniform float a; layout(location=0) out float o;\n"
      "void main(){ float s=0.0; for(int i=0;i<3;i++){ s=s+a; } precise float r=s; o=r; }\n");

   run(ctx, "helper-params",
      "#version 450 core\nuniform vec4 a,b; layout(location=0) out vec4 o;\n"
      "vec4 f(inout vec4 x, vec4 y){ x=x*y; return x+b; }\n"
      "void main(){ precise vec4 r; vec4 t=a; r=f(t,a); o=r; }\n");

   run(ctx, "precise-redeclare",
      "#version 450 core\nuniform vec4 a,b,c; layout(location=0) out vec4 o;\n"
      "void main(){ vec4 r=(a*b)+c; precise r; o=r; }\n");

   run(ctx, "neg-only",
      "#version 450 core\nuniform vec4 a; layout(location=0) out vec4 o;\n"
      "void main(){ precise vec4 r=-a; o=r; }\n");

   run(ctx, "div",
      "#version 450 core\nuniform vec4 a,b; layout(location=0) out vec4 o;\n"
      "void main(){ precise vec4 r=a/b; o=r; }\n");

   run(ctx, "dead-precise-output",
      "#version 450 core\nuniform vec4 a,b,c; layout(location=0) out vec4 o;\n"
      "void main(){ precise vec4 r=(a*b)+c; o=a; }\n");

   run(ctx, "compute-should-not-emit",
      "#version 450 core\nuniform vec4 a,b,c; layout(local_size_x=1) in;\n"
      "layout(binding=0) buffer B { vec4 o; } bb;\n"
      "void main(){ precise vec4 r=(a*b)+c; bb.o=r; }\n", kGlslOptShaderCompute);

   glslopt_cleanup(ctx);
   return 0;
}
