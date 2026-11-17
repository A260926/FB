#include "glsl_optimizer.h"
#include "nir_optimize.h"
#include "compiler/nir/nir.h"
#include <cstdio>
#include "../src/glsl/glsl_optimizer.cpp"
int main(){
   glslopt_ctx* ctx = glslopt_initialize(kGlslTargetOpenGL);
   const char* src =
      "#version 450 core\nlayout(location=0) precise in vec4 a; layout(location=1) in vec4 b; layout(location=0) out vec4 o;\n"
      "void main(){ o=a*b; }\n";
   glslopt_shader* s = glslopt_optimize(ctx, kGlslOptShaderFragment, src, 0);
   nir_print_shader(s->shader->nir, stdout);
   glslopt_shader_delete(s);
   glslopt_cleanup(ctx);
}
