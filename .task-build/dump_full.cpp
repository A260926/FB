#include "glsl_optimizer.h"
#include "nir_optimize.h"
#include "compiler/nir/nir.h"
#include "glsl_parser_extras.h"
#include <cstdio>
#include <cstring>

// pull in the internals the inspect bridge uses
#include "../src/glsl/glsl_optimizer.cpp"

int main(int argc, char** argv) {
   const char* precise = argc > 1 ? argv[1] :
      "#version 450 core\n"
      "uniform vec4 a,b,c; layout(location=0) out vec4 color;\n"
      "void main(){vec4 t=a*b; precise vec4 r=t+c; color=r;}\n";
   glslopt_ctx* ctx = glslopt_initialize(kGlslTargetOpenGL);
   glslopt_shader* s = glslopt_optimize(ctx, kGlslOptShaderFragment, precise, 0);
   std::printf("status=%d\n", glslopt_get_status(s));
   if (!glslopt_get_status(s)) { std::puts(glslopt_get_log(s)); return 1; }
   nir_print_shader(s->shader->nir, stdout);
   std::puts("---- GLSL ----");
   std::puts(glslopt_get_output(s));
   glslopt_shader_delete(s);
   glslopt_cleanup(ctx);
   return 0;
}
