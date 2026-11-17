#include "glsl_optimizer.h"
#include "nir_optimize.h"
#include "compiler/nir/nir.h"
#include "glsl_parser_extras.h"
#include <cstdio>
#include <cstring>

#include "../src/glsl/glsl_optimizer.cpp"

static void run(glslopt_ctx* ctx, const char* tag, const char* src, glslopt_shader_type st) {
   glslopt_shader* s = glslopt_optimize(ctx, st, src, 0);
   std::printf("\n======== %s status=%d\n", tag, glslopt_get_status(s));
   if (!glslopt_get_status(s)) { std::puts(glslopt_get_log(s)); return; }
   nir_print_shader(s->shader->nir, stdout);
   std::puts("---- GLSL ----");
   std::puts(glslopt_get_output(s));
   glslopt_shader_delete(s);
}

int main() {
   glslopt_ctx* ctx = glslopt_initialize(kGlslTargetOpenGL);

   // 1. no precise: reassociation/fma allowed, no precise keyword
   run(ctx, "normal-fma",
      "#version 450 core\nuniform vec4 a,b,c; layout(location=0) out vec4 o;\n"
      "void main(){ o=(a*b)+(a*c); }\n", kGlslOptShaderFragment);

   // 2. precise root only on one output; other output stays optimizable
   run(ctx, "two-outputs",
      "#version 450 core\nuniform vec4 a,b,c; layout(location=0) out vec4 p; layout(location=1) out vec4 q;\n"
      "void main(){ precise vec4 r=(a*b)+c; p=r; q=(a*b)+(a*c); }\n", kGlslOptShaderFragment);

   // 3. partial component precise
   run(ctx, "partial-comp",
      "#version 450 core\nuniform vec4 a,b,c; layout(location=0) out vec4 o;\n"
      "void main(){ vec4 t=a*b; precise float r=t.x+c.x; o=vec4(r,t.y,t.z,t.w); }\n",
      kGlslOptShaderFragment);

   // 3b. while loop with carried precise value, negation and swizzle
   run(ctx, "while-neg-swz",
      "#version 450 core\nuniform vec4 a; uniform int n; layout(location=0) out vec4 o;\n"
      "void main(){ vec4 s=vec4(0.0); int i=0; while(i<n){ s=-s+a.yxwz; i++; } precise vec4 r=s; o=r; }\n",
      kGlslOptShaderFragment);

   // 3c. dynamic array index
   run(ctx, "dyn-array",
      "#version 450 core\nuniform vec4 a,b; uniform int k; layout(location=0) out vec4 o;\n"
      "void main(){ vec4 v[2]; v[0]=a*b; v[1]=a+b; precise vec4 r=v[k]; o=r; }\n",
      kGlslOptShaderFragment);

   // 3d. explicit fma builtin must not be extended, but surrounding ops are
   run(ctx, "fma-builtin",
      "#version 450 core\nuniform vec4 a,b,c,d; layout(location=0) out vec4 o;\n"
      "void main(){ precise vec4 r=fma(a,b,c)+d; o=r; }\n",
      kGlslOptShaderFragment);

   // 3e. integer math mixed in: not protected, float output is
   run(ctx, "int-mix",
      "#version 450 core\nuniform vec4 a; uniform int k; layout(location=0) out vec4 o;\n"
      "void main(){ int j=k+1; precise vec4 r=a*float(j); o=r; }\n",
      kGlslOptShaderFragment);

   // 3f. vertex stage
   run(ctx, "vertex",
      "#version 450 core\nlayout(location=0) in vec4 a; layout(location=1) in vec4 b;\n"
      "void main(){ precise vec4 r=a*b+a; gl_Position=r; }\n", kGlslOptShaderVertex);

   // 4. loop carried
   run(ctx, "loop",
      "#version 450 core\nuniform float a; uniform int n; layout(location=0) out float o;\n"
      "void main(){ float s=0.0; for(int i=0;i<n;i++){ s=s+a; } precise float r=s; o=r; }\n",
      kGlslOptShaderFragment);

   // 5. branch merge
   run(ctx, "branch",
      "#version 450 core\nuniform vec4 a,b,c; uniform bool k; layout(location=0) out vec4 o;\n"
      "void main(){ vec4 r; if(k) r=a*b; else r=a+c; precise vec4 q=r; o=q; }\n",
      kGlslOptShaderFragment);

   // 6. local array constant index
   run(ctx, "array",
      "#version 450 core\nuniform vec4 a,b; layout(location=0) out vec4 o;\n"
      "void main(){ vec4 v[2]; v[0]=a*b; v[1]=a+b; precise vec4 r=v[0]; o=r; }\n",
      kGlslOptShaderFragment);

   // 7. helper in/out/inout
   run(ctx, "helper",
      "#version 450 core\nuniform vec4 a,b,c; layout(location=0) out vec4 o;\n"
      "void h(in vec4 x, in vec4 y, out vec4 z){ z=(x*y)+c; }\n"
      "void main(){ precise vec4 r; h(a,b,r); o=r; }\n",
      kGlslOptShaderFragment);

   glslopt_cleanup(ctx);
   return 0;
}
